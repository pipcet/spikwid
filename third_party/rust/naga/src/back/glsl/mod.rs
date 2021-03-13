//! OpenGL shading language backend
//!
//! The main structure is [`Writer`](Writer), it maintains internal state that is used
//! to output a [`Module`](crate::Module) into glsl
//!
//! # Supported versions
//! ### Core
//! - 330
//! - 400
//! - 410
//! - 420
//! - 430
//! - 450
//! - 460
//!
//! ### ES
//! - 300
//! - 310
//!

// GLSL is mostly a superset of C but it also removes some parts of it this is a list of relevant
// aspects for this backend.
//
// The most notable change is the introduction of the version preprocessor directive that must
// always be the first line of a glsl file and is written as
// `#version number profile`
// `number` is the version itself (i.e. 300) and `profile` is the
// shader profile we only support "core" and "es", the former is used in desktop applications and
// the later is used in embedded contexts, mobile devices and browsers. Each one as it's own
// versions (at the time of writing this the latest version for "core" is 460 and for "es" is 320)
//
// Other important preprocessor addition is the extension directive which is written as
// `#extension name: behaviour`
// Extensions provide increased features in a plugin fashion but they aren't required to be
// supported hence why they are called extensions, that's why `behaviour` is used it specifies
// wether the extension is strictly required or if it should only be enabled if needed. In our case
// when we use extensions we set behaviour to `require` always.
//
// The only thing that glsl removes that makes a difference are pointers.
//
// Additions that are relevant for the backend are the discard keyword, the introduction of
// vector, matrices, samplers, image types and functions that provide common shader operations

pub use features::Features;

use crate::{
    proc::{analyzer::Analysis, NameKey, Namer, ResolveContext, Typifier, TypifyError},
    Arena, ArraySize, BinaryOperator, Binding, BuiltIn, Bytes, ConservativeDepth, Constant,
    ConstantInner, DerivativeAxis, Expression, FastHashMap, Function, GlobalVariable, Handle,
    ImageClass, Interpolation, LocalVariable, Module, RelationalFunction, ScalarKind, ScalarValue,
    ShaderStage, Statement, StorageAccess, StorageClass, StorageFormat, StructMember, Type,
    TypeInner, UnaryOperator,
};
use features::FeaturesManager;
use std::{
    cmp::Ordering,
    fmt,
    io::{Error as IoError, Write},
};
use thiserror::Error;

/// Contains the features related code and the features querying method
mod features;
/// Contains a constant with a slice of all the reserved keywords RESERVED_KEYWORDS
mod keywords;

/// List of supported core glsl versions
pub const SUPPORTED_CORE_VERSIONS: &[u16] = &[330, 400, 410, 420, 430, 440, 450];
/// List of supported es glsl versions
pub const SUPPORTED_ES_VERSIONS: &[u16] = &[300, 310, 320];
const INDENT: &str = "    ";

/// glsl version
#[derive(Debug, Copy, Clone, PartialEq)]
pub enum Version {
    /// `core` glsl
    Desktop(u16),
    /// `es` glsl
    Embedded(u16),
}

impl Version {
    /// Returns true if self is `Version::Embedded` (i.e. is a es version)
    fn is_es(&self) -> bool {
        match self {
            Version::Desktop(_) => false,
            Version::Embedded(_) => true,
        }
    }

    /// Checks the list of currently supported versions and returns true if it contains the
    /// specified version
    ///
    /// # Notes
    /// As an invalid version number will never be added to the supported version list
    /// so this also checks for version validity
    fn is_supported(&self) -> bool {
        match self {
            Version::Desktop(v) => SUPPORTED_CORE_VERSIONS.contains(v),
            Version::Embedded(v) => SUPPORTED_ES_VERSIONS.contains(v),
        }
    }
}

impl PartialOrd for Version {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        match (*self, *other) {
            (Version::Desktop(x), Version::Desktop(y)) => Some(x.cmp(&y)),
            (Version::Embedded(x), Version::Embedded(y)) => Some(x.cmp(&y)),
            _ => None,
        }
    }
}

impl fmt::Display for Version {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Version::Desktop(v) => write!(f, "{} core", v),
            Version::Embedded(v) => write!(f, "{} es", v),
        }
    }
}

/// Structure that contains the configuration used in the [`Writer`](Writer)
#[derive(Debug, Clone)]
pub struct Options {
    /// The glsl version to be used
    pub version: Version,
    /// The stage of the entry point
    pub shader_stage: ShaderStage,
    /// The name of the entry point
    ///
    /// If no entry point that matches is found a error will be thrown while creating a new instance
    /// of [`Writer`](struct.Writer.html)
    pub entry_point: String,
}

/// Structure that connects a texture to a sampler or not
///
/// glsl pre vulkan has no concept of separate textures and samplers instead everything is a
/// `gsamplerN` where `g` is the scalar type and `N` is the dimension, but naga uses separate textures
/// and samplers in the IR so the backend produces a [`HashMap`](crate::FastHashMap) with the texture name
/// as a key and a [`TextureMapping`](TextureMapping) as a value this way the user knows where to bind.
///
/// [`Storage`](crate::ImageClass::Storage) images produce `gimageN` and don't have an associated sampler
/// so the [`sampler`](Self::sampler) field will be [`None`](std::option::Option::None)
#[derive(Debug, Clone)]
pub struct TextureMapping {
    /// Handle to the image global variable
    pub texture: Handle<GlobalVariable>,
    /// Handle to the associated sampler global variable if it exists
    pub sampler: Option<Handle<GlobalVariable>>,
}

/// Stores the current function type (either a regular function or an entry point)
///
/// Also stores data needed to identify it (handle for a regular function or index for an entry point)
enum FunctionType {
    /// A regular function and it's handle
    Function(Handle<Function>),
    /// A entry point and it's index
    EntryPoint(crate::proc::EntryPointIndex),
}

/// Helper structure that stores data needed when writing the function
struct FunctionCtx<'a, 'b> {
    /// The current function being written
    func: FunctionType,
    /// The expression arena of the current function being written
    expressions: &'a Arena<Expression>,
    /// A typifier that has already resolved all expressions in the function being written
    typifier: &'b Typifier,
}

impl<'a, 'b> FunctionCtx<'a, 'b> {
    /// Helper method that generates a [`NameKey`](crate::proc::NameKey) for a local in the current function
    fn name_key(&self, local: Handle<LocalVariable>) -> NameKey {
        match self.func {
            FunctionType::Function(handle) => NameKey::FunctionLocal(handle, local),
            FunctionType::EntryPoint(idx) => NameKey::EntryPointLocal(idx, local),
        }
    }

    /// Helper method that retrieves the name of the argument in the current function
    ///
    /// # Panics
    /// - If the function is an entry point
    /// - If the function arguments are less or equal to `arg`
    /// - If `names` hasn't been filled properly
    fn get_arg<'c>(&self, arg: u32, names: &'c FastHashMap<NameKey, String>) -> &'c str {
        match self.func {
            FunctionType::Function(handle) => &names[&NameKey::FunctionArgument(handle, arg)],
            FunctionType::EntryPoint(_) => unreachable!(),
        }
    }
}

/// Helper structure that generates a number
#[derive(Default)]
struct IdGenerator(u32);

impl IdGenerator {
    /// Generates a number that's guaranteed to be unique for this `IdGenerator`
    fn generate(&mut self) -> u32 {
        // It's just an increasing number but it does the job
        let ret = self.0;
        self.0 += 1;
        ret
    }
}

/// Shorthand result used internally by the backend
type BackendResult = Result<(), Error>;

/// A glsl compilation error.
#[derive(Debug, Error)]
pub enum Error {
    /// A error occurred while writing to the output
    #[error("I/O error")]
    IoError(#[from] IoError),
    /// The [`Module`](crate::Module) failed type resolution
    #[error("Type error")]
    Type(#[from] TypifyError),
    /// The specified [`Version`](Version) doesn't have all required [`Features`](super)
    ///
    /// Contains the missing [`Features`](Features)
    #[error("The selected version doesn't support {0:?}")]
    MissingFeatures(Features),
    /// [`StorageClass::PushConstant`](crate::StorageClass::PushConstant) was used and isn't
    /// supported in the glsl backend
    #[error("Push constants aren't supported")]
    PushConstantNotSupported,
    /// The specified [`Version`](Version) isn't supported
    #[error("The specified version isn't supported")]
    VersionNotSupported,
    /// The entry point couldn't be found
    #[error("The requested entry point couldn't be found")]
    EntryPointNotFound,
    /// A call was made to an unsupported external
    #[error("A call was made to an unsupported external: {0}")]
    UnsupportedExternal(String),
    /// A scalar with an unsupported width was requested
    #[error("A scalar with an unsupported width was requested: {0:?} {1:?}")]
    UnsupportedScalar(ScalarKind, Bytes),
    /// [`Interpolation::Patch`](crate::Interpolation::Patch) isn't supported
    #[error("Patch interpolation isn't supported")]
    PatchInterpolationNotSupported,
    /// A image was used with multiple samplers, this isn't supported
    #[error("A image was used with multiple samplers")]
    ImageMultipleSamplers,
    #[error("{0}")]
    Custom(String),
}

/// Main structure of the glsl backend responsible for all code generation
pub struct Writer<'a, W> {
    // Inputs
    /// The module being written
    module: &'a Module,
    /// The module analysis.
    analysis: &'a Analysis,
    /// The output writer
    out: W,
    /// User defined configuration to be used
    options: &'a Options,

    // Internal State
    /// Features manager used to store all the needed features and write them
    features: FeaturesManager,
    /// A map with all the names needed for writing the module
    /// (generated by a [`Namer`](crate::proc::Namer))
    names: FastHashMap<NameKey, String>,
    /// The selected entry point
    entry_point: &'a crate::EntryPoint,
    /// The index of the selected entry point
    entry_point_idx: crate::proc::EntryPointIndex,
    /// Used to generate a unique number for blocks
    block_id: IdGenerator,
}

impl<'a, W: Write> Writer<'a, W> {
    /// Creates a new [`Writer`](Writer) instance
    ///
    /// # Errors
    /// - If the version specified isn't supported (or invalid)
    /// - If the entry point couldn't be found on the module
    /// - If the version specified doesn't support some used features
    pub fn new(
        out: W,
        module: &'a Module,
        analysis: &'a Analysis,
        options: &'a Options,
    ) -> Result<Self, Error> {
        // Check if the requested version is supported
        if !options.version.is_supported() {
            log::error!("Version {}", options.version);
            return Err(Error::VersionNotSupported);
        }

        // Try to find the entry point and corresponding index
        let (ep_idx, (_, ep)) = module
            .entry_points
            .iter()
            .enumerate()
            .find(|(_, ((stage, name), _))| {
                options.shader_stage == *stage && &options.entry_point == name
            })
            .ok_or(Error::EntryPointNotFound)?;

        // Generate a map with names required to write the module
        let mut names = FastHashMap::default();
        Namer::default().reset(module, keywords::RESERVED_KEYWORDS, &mut names);

        // Build the instance
        let mut this = Self {
            module,
            analysis,
            out,
            options,

            features: FeaturesManager::new(),
            names,
            entry_point: ep,
            entry_point_idx: ep_idx as u16,

            block_id: IdGenerator::default(),
        };

        // Find all features required to print this module
        this.collect_required_features()?;

        Ok(this)
    }

    /// Writes the [`Module`](crate::Module) as glsl to the output
    ///
    /// # Notes
    /// If an error occurs while writing, the output might have been written partially
    ///
    /// # Panics
    /// Might panic if the module is invalid
    pub fn write(&mut self) -> Result<FastHashMap<String, TextureMapping>, Error> {
        // We use `writeln!(self.out)` throughout the write to add newlines
        // to make the output more readable

        let es = self.options.version.is_es();

        // Write the version (It must be the first thing or it isn't a valid glsl output)
        writeln!(self.out, "#version {}", self.options.version)?;
        // Write all the needed extensions
        //
        // This used to be the last thing being written as it allowed to search for features while
        // writing the module saving some loops but some older versions (420 or less) required the
        // extensions to appear before being used, even though extensions are part of the
        // preprocessor not the processor ¯\_(ツ)_/¯
        self.features.write(self.options.version, &mut self.out)?;

        // glsl es requires a precision to be specified for floats
        // TODO: Should this be user configurable?
        if es {
            writeln!(self.out)?;
            writeln!(self.out, "precision highp float;")?;
            writeln!(self.out)?;
        }

        // Enable early depth tests if needed
        if let Some(depth_test) = self.entry_point.early_depth_test {
            writeln!(self.out, "layout(early_fragment_tests) in;")?;

            if let Some(conservative) = depth_test.conservative {
                writeln!(
                    self.out,
                    "layout (depth_{}) out float gl_FragDepth;",
                    match conservative {
                        ConservativeDepth::GreaterEqual => "greater",
                        ConservativeDepth::LessEqual => "less",
                        ConservativeDepth::Unchanged => "unchanged",
                    }
                )?;
            }
            writeln!(self.out)?;
        }

        // Write all structs
        //
        // This are always ordered because of the IR is structured in a way that you can't make a
        // struct without adding all of it's members first
        for (handle, ty) in self.module.types.iter() {
            if let TypeInner::Struct {
                block: _,
                ref members,
            } = ty.inner
            {
                self.write_struct(handle, members)?
            }
        }

        let ep_info = self
            .analysis
            .get_entry_point(self.options.shader_stage, &self.options.entry_point);

        // Write the globals
        //
        // We filter all globals that aren't used by the selected entry point as they might be
        // interfere with each other (i.e. two globals with the same location but different with
        // different classes)
        for (handle, global) in self.module.global_variables.iter() {
            if ep_info[handle].is_empty() {
                continue;
            }

            // Skip builtins
            // TODO: Write them if they have modifiers
            if let Some(crate::Binding::BuiltIn(_)) = global.binding {
                continue;
            }

            match self.module.types[global.ty].inner {
                // We treat images separately because they might require
                // writing the storage format
                TypeInner::Image {
                    dim,
                    arrayed,
                    class,
                } => {
                    // Write the storage format if needed
                    if let TypeInner::Image {
                        class: ImageClass::Storage(format),
                        ..
                    } = self.module.types[global.ty].inner
                    {
                        write!(self.out, "layout({}) ", glsl_storage_format(format))?;
                    }

                    // Write the storage access modifier
                    //
                    // glsl allows adding both `readonly` and `writeonly` but this means that
                    // they can only be used to query information about the image which isn't what
                    // we want here so when storage access is both `LOAD` and `STORE` add no modifiers
                    if global.storage_access == StorageAccess::LOAD {
                        write!(self.out, "readonly ")?;
                    } else if global.storage_access == StorageAccess::STORE {
                        write!(self.out, "writeonly ")?;
                    }

                    // All images in glsl are `uniform`
                    // The trailing space is important
                    write!(self.out, "uniform ")?;

                    // write the type
                    //
                    // This is way we need the leading space because `write_image_type` doesn't add
                    // any spaces at the beginning or end
                    self.write_image_type(dim, arrayed, class)?;

                    // Finally write the name and end the global with a `;`
                    // The leading space is important
                    writeln!(self.out, " {};", self.get_global_name(handle, global))?;

                    writeln!(self.out)?;
                }
                // glsl has no concept of samplers so we just ignore it
                TypeInner::Sampler { .. } => continue,
                // All other globals are written by `write_global`
                _ => self.write_global(handle, global)?,
            }
        }
        // Write all regular functions
        for (handle, function) in self.module.functions.iter() {
            // Check that the function doesn't use globals that aren't supported
            // by the current entry point
            if !ep_info.dominates_global_use(&self.analysis[handle]) {
                continue;
            }

            // We also `clone` to satisfy the borrow checker
            let name = self.names[&NameKey::Function(handle)].clone();

            // Write the function
            self.write_function(FunctionType::Function(handle), function, name)?;

            writeln!(self.out)?;
        }

        self.write_function(
            FunctionType::EntryPoint(self.entry_point_idx),
            &self.entry_point.function,
            "main",
        )?;

        // Collect all of the texture mappings and return them to the user
        self.collect_texture_mapping()
    }

    /// Helper method used to write non image/sampler types
    ///
    /// # Notes
    /// Adds no trailing or leading whitespace
    ///
    /// # Panics
    /// - If type is either a image or sampler
    /// - If it's an Array with a [`ArraySize::Constant`](crate::ArraySize::Constant) with a
    /// constant that isn't [`Uint`](crate::ConstantInner::Uint)
    fn write_type(&mut self, ty: Handle<Type>) -> BackendResult {
        match self.module.types[ty].inner {
            // Scalars are simple we just get the full name from `glsl_scalar`
            TypeInner::Scalar { kind, width } => {
                write!(self.out, "{}", glsl_scalar(kind, width)?.full)?
            }
            // Vectors are just `gvecN` where `g` is the scalar prefix and `N` is the vector size
            TypeInner::Vector { size, kind, width } => write!(
                self.out,
                "{}vec{}",
                glsl_scalar(kind, width)?.prefix,
                size as u8
            )?,
            // Matrices are written with `gmatMxN` where `g` is the scalar prefix (only floats and
            // doubles are allowed), `M` is the columns count and `N` is the rows count
            //
            // glsl supports a matrix shorthand `gmatN` where `N` = `M` but it doesn't justify the
            // extra branch to write matrices this way
            TypeInner::Matrix {
                columns,
                rows,
                width,
            } => write!(
                self.out,
                "{}mat{}x{}",
                glsl_scalar(ScalarKind::Float, width)?.prefix,
                columns as u8,
                rows as u8
            )?,
            // glsl has no pointer types so just write types as normal and loads are skipped
            TypeInner::Pointer { base, .. } => self.write_type(base)?,
            // Arrays are written as `base[size]`
            TypeInner::Array { base, size, .. } => {
                self.write_type(base)?;

                write!(self.out, "[")?;

                // Write the array size
                // Writes nothing if `ArraySize::Dynamic`
                // Panics if `ArraySize::Constant` has a constant that isn't an uint
                match size {
                    ArraySize::Constant(const_handle) => {
                        match self.module.constants[const_handle].inner {
                            ConstantInner::Scalar {
                                width: _,
                                value: ScalarValue::Uint(size),
                            } => write!(self.out, "{}", size)?,
                            _ => unreachable!(),
                        }
                    }
                    ArraySize::Dynamic => (),
                }

                write!(self.out, "]")?
            }
            // glsl structs are written as just the struct name if it isn't a block
            //
            // If it's a block we need to write `block_name { members }` where `block_name` must be
            // unique between blocks and structs so we add `_block_ID` where `ID` is a `IdGenerator`
            // generated number so it's unique and `members` are the same as in a struct
            TypeInner::Struct { block, ref members } => {
                // Get the struct name
                let name = &self.names[&NameKey::Type(ty)];

                if block {
                    // Write the block name, it's just the struct name appended with `_block_ID`
                    writeln!(self.out, "{}_block_{} {{", name, self.block_id.generate())?;

                    // Write the block members
                    for (idx, member) in members.iter().enumerate() {
                        // Add a tab for indentation (readability only)
                        write!(self.out, "{}", INDENT)?;
                        // Write the member type
                        self.write_type(member.ty)?;

                        // Finish the member with the name, a semicolon and a newline
                        // The leading space is important
                        writeln!(
                            self.out,
                            " {};",
                            &self.names[&NameKey::StructMember(ty, idx as u32)]
                        )?;
                    }

                    // Close braces
                    write!(self.out, "}}")?
                } else {
                    // Write the struct name
                    write!(self.out, "{}", name)?
                }
            }
            // Panic if either Image or Sampler is being written
            //
            // Write all variants instead of `_` so that if new variants are added a
            // no exhaustiveness error is thrown
            TypeInner::Image { .. } | TypeInner::Sampler { .. } => unreachable!(),
        }

        Ok(())
    }

    /// Helper method to write a image type
    ///
    /// # Notes
    /// Adds no leading or trailing whitespace
    fn write_image_type(
        &mut self,
        dim: crate::ImageDimension,
        arrayed: bool,
        class: ImageClass,
    ) -> BackendResult {
        // glsl images consist of four parts the scalar prefix, the image "type", the dimensions
        // and modifiers
        //
        // There exists two image types
        // - sampler - for sampled images
        // - image - for storage images
        //
        // There are three possible modifiers that can be used together and must be written in
        // this order to be valid
        // - MS - used if it's a multisampled image
        // - Array - used if it's an image array
        // - Shadow - used if it's a depth image

        let (base, kind, ms, comparison) = match class {
            ImageClass::Sampled { kind, multi: true } => ("sampler", kind, "MS", ""),
            ImageClass::Sampled { kind, multi: false } => ("sampler", kind, "", ""),
            ImageClass::Depth => ("sampler", crate::ScalarKind::Float, "", "Shadow"),
            ImageClass::Storage(format) => ("image", format.into(), "", ""),
        };

        write!(
            self.out,
            "{}{}{}{}{}{}",
            glsl_scalar(kind, 4)?.prefix,
            base,
            glsl_dimension(dim),
            ms,
            if arrayed { "Array" } else { "" },
            comparison
        )?;

        Ok(())
    }

    /// Helper method used to write non images/sampler globals
    ///
    /// # Notes
    /// Adds a newline
    ///
    /// # Panics
    /// If the global has type sampler
    fn write_global(
        &mut self,
        handle: Handle<GlobalVariable>,
        global: &GlobalVariable,
    ) -> BackendResult {
        // Write the storage access modifier
        //
        // glsl allows adding both `readonly` and `writeonly` but this means that
        // they can only be used to query information about the resource which isn't what
        // we want here so when storage access is both `LOAD` and `STORE` add no modifiers
        if global.storage_access == StorageAccess::LOAD {
            write!(self.out, "readonly ")?;
        } else if global.storage_access == StorageAccess::STORE {
            write!(self.out, "writeonly ")?;
        }

        // Write the interpolation modifier if needed
        //
        // We ignore all interpolation modifiers that aren't used in input globals in fragment
        // shaders or output globals in vertex shaders
        //
        // TODO: Should this throw an error?
        if let Some(interpolation) = global.interpolation {
            match (self.options.shader_stage, global.class) {
                (ShaderStage::Fragment, StorageClass::Input)
                | (ShaderStage::Vertex, StorageClass::Output) => {
                    write!(self.out, "{} ", glsl_interpolation(interpolation)?)?;
                }
                _ => (),
            };
        }

        // Write the storage class
        // Trailing space is important
        write!(self.out, "{} ", glsl_storage_class(global.class))?;

        // Write the type
        // `write_type` adds no leading or trailing spaces
        self.write_type(global.ty)?;

        // Finally write the global name and end the global with a `;` and a newline
        // Leading space is important
        writeln!(self.out, " {};", self.get_global_name(handle, global))?;
        writeln!(self.out)?;

        Ok(())
    }

    /// Helper method used to get a name for a global
    ///
    /// Globals have different naming schemes depending on their binding:
    /// - Globals without bindings use the name from the [`Namer`](crate::proc::Namer)
    /// - Globals with builtin bindings get the from [`glsl_built_in`](glsl_built_in)
    /// - Globals with location bindings are named `_location_X` where `X` is the location
    /// - Globals with resource binding are named `_group_X_binding_Y` where `X`
    ///   is the group and `Y` is the binding
    fn get_global_name(&self, handle: Handle<GlobalVariable>, global: &GlobalVariable) -> String {
        match global.binding {
            Some(Binding::Location(location)) => {
                format!(
                    "_location_{}{}",
                    location,
                    match (self.options.shader_stage, global.class) {
                        (ShaderStage::Fragment, StorageClass::Input) => "_vs",
                        (ShaderStage::Vertex, StorageClass::Output) => "_vs",
                        _ => "",
                    }
                )
            }
            Some(Binding::Resource { group, binding }) => {
                format!("_group_{}_binding_{}", group, binding)
            }
            Some(Binding::BuiltIn(built_in)) => glsl_built_in(built_in).to_string(),
            None => self.names[&NameKey::GlobalVariable(handle)].clone(),
        }
    }

    /// Helper method used to write functions (both entry points and regular functions)
    ///
    /// # Notes
    /// Adds a newline
    fn write_function<N: AsRef<str>>(
        &mut self,
        ty: FunctionType,
        func: &Function,
        name: N,
    ) -> BackendResult {
        // Create a new typifier and resolve all types for the current function
        let mut typifier = Typifier::new();
        typifier.resolve_all(
            &func.expressions,
            &self.module.types,
            &ResolveContext {
                constants: &self.module.constants,
                global_vars: &self.module.global_variables,
                local_vars: &func.local_variables,
                functions: &self.module.functions,
                arguments: &func.arguments,
            },
        )?;

        // Create a function context for the function being written
        let ctx = FunctionCtx {
            func: ty,
            expressions: &func.expressions,
            typifier: &typifier,
        };

        // Write the function header
        //
        // glsl headers are the same as in c:
        // `ret_type name(args)`
        // `ret_type` is the return type
        // `name` is the function name
        // `args` is a comma separated list of `type name`
        //  | - `type` is the argument type
        //  | - `name` is the argument name

        // Start by writing the return type if any otherwise write void
        // This is the only place where `void` is a valid type
        // (though it's more a keyword than a type)
        if let Some(ty) = func.return_type {
            self.write_type(ty)?;
        } else {
            write!(self.out, "void")?;
        }

        // Write the function name and open parentheses for the argument list
        write!(self.out, " {}(", name.as_ref())?;

        // Write the comma separated argument list
        //
        // We need access to `Self` here so we use the reference passed to the closure as an
        // argument instead of capturing as that would cause a borrow checker error
        self.write_slice(&func.arguments, |this, i, arg| {
            // Write the argument type
            // `write_type` adds no trailing spaces
            this.write_type(arg.ty)?;

            // Write the argument name
            // The leading space is important
            write!(this.out, " {}", ctx.get_arg(i, &this.names))?;

            Ok(())
        })?;

        // Close the parentheses and open braces to start the function body
        writeln!(self.out, ") {{")?;

        // Write all function locals
        // Locals are `type name (= init)?;` where the init part (including the =) are optional
        //
        // Always adds a newline
        for (handle, local) in func.local_variables.iter() {
            // Write indentation (only for readability) and the type
            // `write_type` adds no trailing space
            write!(self.out, "{}", INDENT)?;
            self.write_type(local.ty)?;

            // Write the local name
            // The leading space is important
            write!(self.out, " {}", self.names[&ctx.name_key(handle)])?;

            // Write the local initializer if needed
            if let Some(init) = local.init {
                // Put the equal signal only if there's a initializer
                // The leading and trailing spaces aren't needed but help with readability
                write!(self.out, " = ")?;

                // Write the constant
                // `write_constant` adds no trailing or leading space/newline
                self.write_constant(&self.module.constants[init])?;
            }

            // Finish the local with `;` and add a newline (only for readability)
            writeln!(self.out, ";")?
        }

        // Write the function body (statement list)
        for sta in func.body.iter() {
            // Write a statement, the indentation should always be 1 when writing the function body
            // `write_stmt` adds a newline
            self.write_stmt(sta, &ctx, 1)?;
        }

        // Close braces and add a newline
        writeln!(self.out, "}}")?;

        Ok(())
    }

    /// Helper method that writes a list of comma separated `T` with a writer function `F`
    ///
    /// The writer function `F` receives a mutable reference to `self` that if needed won't cause
    /// borrow checker issues (using for example a closure with `self` will cause issues), the
    /// second argument is the 0 based index of the element on the list, and the last element is
    /// a reference to the element `T` being written
    ///
    /// # Notes
    /// - Adds no newlines or leading/trailing whitespace
    /// - The last element won't have a trailing `,`
    fn write_slice<T, F: FnMut(&mut Self, u32, &T) -> BackendResult>(
        &mut self,
        data: &[T],
        mut f: F,
    ) -> BackendResult {
        // Loop trough `data` invoking `f` for each element
        for (i, item) in data.iter().enumerate() {
            f(self, i as u32, item)?;

            // Only write a comma if isn't the last element
            if i != data.len().saturating_sub(1) {
                // The leading space is for readability only
                write!(self.out, ", ")?;
            }
        }

        Ok(())
    }

    /// Helper method used to write constants
    ///
    /// # Notes
    /// Adds no newlines or leading/trailing whitespace
    fn write_constant(&mut self, constant: &Constant) -> BackendResult {
        match constant.inner {
            ConstantInner::Scalar {
                width: _,
                ref value,
            } => match *value {
                // Signed integers don't need anything special
                ScalarValue::Sint(int) => write!(self.out, "{}", int)?,
                // Unsigned integers need a `u` at the end
                //
                // While `core` doesn't necessarily need it, it's allowed and since `es` needs it we
                // always write it as the extra branch wouldn't have any benefit in readability
                ScalarValue::Uint(int) => write!(self.out, "{}u", int)?,
                // Floats are written using `Debug` instead of `Display` because it always appends the
                // decimal part even it's zero which is needed for a valid glsl float constant
                ScalarValue::Float(float) => write!(self.out, "{:?}", float)?,
                // Booleans are either `true` or `false` so nothing special needs to be done
                ScalarValue::Bool(boolean) => write!(self.out, "{}", boolean)?,
            },
            // Composite constant are created using the same syntax as compose
            // `type(components)` where `components` is a comma separated list of constants
            ConstantInner::Composite { ty, ref components } => {
                self.write_type(ty)?;
                write!(self.out, "(")?;

                // Write the comma separated constants
                self.write_slice(components, |this, _, arg| {
                    this.write_constant(&this.module.constants[*arg])
                })?;

                write!(self.out, ")")?
            }
        }

        Ok(())
    }

    /// Helper method used to write structs
    ///
    /// # Notes
    /// Ends in a newline
    fn write_struct(&mut self, handle: Handle<Type>, members: &[StructMember]) -> BackendResult {
        // glsl structs are written as in C
        // `struct name() { members };`
        //  | `struct` is a keyword
        //  | `name` is the struct name
        //  | `members` is a semicolon separated list of `type name`
        //      | `type` is the member type
        //      | `name` is the member name

        writeln!(self.out, "struct {} {{", self.names[&NameKey::Type(handle)])?;

        for (idx, member) in members.iter().enumerate() {
            // The indentation is only for readability
            write!(self.out, "{}", INDENT)?;

            // Write the member type
            // Adds no trailing space
            self.write_type(member.ty)?;

            // Write the member name and put a semicolon
            // The leading space is important
            // All members must have a semicolon even the last one
            writeln!(
                self.out,
                " {};",
                self.names[&NameKey::StructMember(handle, idx as u32)]
            )?;
        }

        writeln!(self.out, "}};")?;
        writeln!(self.out)?;

        Ok(())
    }

    /// Helper method used to write statements
    ///
    /// # Notes
    /// Always adds a newline
    fn write_stmt(
        &mut self,
        sta: &Statement,
        ctx: &FunctionCtx<'_, '_>,
        indent: usize,
    ) -> BackendResult {
        // The indentation is only for readability
        write!(self.out, "{}", INDENT.repeat(indent))?;

        match *sta {
            // Blocks are simple we just need to write the block statements between braces
            // We could also just print the statements but this is more readable and maps more
            // closely to the IR
            Statement::Block(ref block) => {
                writeln!(self.out, "{{")?;
                for sta in block.iter() {
                    // Increase the indentation to help with readability
                    self.write_stmt(sta, ctx, indent + 1)?
                }
                writeln!(self.out, "{}}}", INDENT.repeat(indent))?
            }
            // Ifs are written as in C:
            // ```
            // if(condition) {
            //  accept
            // } else {
            //  reject
            // }
            // ```
            Statement::If {
                condition,
                ref accept,
                ref reject,
            } => {
                write!(self.out, "if(")?;
                self.write_expr(condition, ctx)?;
                writeln!(self.out, ") {{")?;

                for sta in accept {
                    // Increase indentation to help with readability
                    self.write_stmt(sta, ctx, indent + 1)?;
                }

                // If there are no statements in the reject block we skip writing it
                // This is only for readability
                if !reject.is_empty() {
                    writeln!(self.out, "{}}} else {{", INDENT.repeat(indent))?;

                    for sta in reject {
                        // Increase indentation to help with readability
                        self.write_stmt(sta, ctx, indent + 1)?;
                    }
                }

                writeln!(self.out, "{}}}", INDENT.repeat(indent))?
            }
            // Switch are written as in C:
            // ```
            // switch (selector) {
            //      // Fallthrough
            //      case label:
            //          block
            //      // Non fallthrough
            //      case label:
            //          block
            //          break;
            //      default:
            //          block
            //  }
            //  ```
            //  Where the `default` case happens isn't important but we put it last
            //  so that we don't need to print a `break` for it
            Statement::Switch {
                selector,
                ref cases,
                ref default,
            } => {
                // Start the switch
                write!(self.out, "switch(")?;
                self.write_expr(selector, ctx)?;
                writeln!(self.out, ") {{")?;

                // Write all cases
                for case in cases {
                    writeln!(
                        self.out,
                        "{}case {}:",
                        INDENT.repeat(indent + 1),
                        case.value
                    )?;

                    for sta in case.body.iter() {
                        self.write_stmt(sta, ctx, indent + 2)?;
                    }

                    // Write `break;` if the block isn't fallthrough
                    if case.fall_through {
                        writeln!(self.out, "{}break;", INDENT.repeat(indent + 2))?;
                    }
                }

                // Only write the default block if the block isn't empty
                // Writing default without a block is valid but it's more readable this way
                if !default.is_empty() {
                    writeln!(self.out, "{}default:", INDENT.repeat(indent + 1))?;

                    for sta in default {
                        self.write_stmt(sta, ctx, indent + 2)?;
                    }
                }

                writeln!(self.out, "{}}}", INDENT.repeat(indent))?
            }
            // Loops in naga IR are based on wgsl loops, glsl can emulate the behaviour by using a
            // while true loop and appending the continuing block to the body resulting on:
            // ```
            // while(true) {
            //  body
            //  continuing
            // }
            // ```
            Statement::Loop {
                ref body,
                ref continuing,
            } => {
                writeln!(self.out, "while(true) {{")?;

                for sta in body.iter().chain(continuing.iter()) {
                    self.write_stmt(sta, ctx, indent + 1)?;
                }

                writeln!(self.out, "{}}}", INDENT.repeat(indent))?
            }
            // Break, continue and return as written as in C
            // `break;`
            Statement::Break => writeln!(self.out, "break;")?,
            // `continue;`
            Statement::Continue => writeln!(self.out, "continue;")?,
            // `return expr;`, `expr` is optional
            Statement::Return { value } => {
                write!(self.out, "return")?;
                // Write the expression to be returned if needed
                if let Some(expr) = value {
                    write!(self.out, " ")?;
                    self.write_expr(expr, ctx)?;
                }
                writeln!(self.out, ";")?;
            }
            // This is one of the places were glsl adds to the syntax of C in this case the discard
            // keyword which ceases all further processing in a fragment shader, it's called OpKill
            // in spir-v that's why it's called `Statement::Kill`
            Statement::Kill => writeln!(self.out, "discard;")?,
            // Stores in glsl are just variable assignments written as `pointer = value;`
            Statement::Store { pointer, value } => {
                self.write_expr(pointer, ctx)?;
                write!(self.out, " = ")?;
                self.write_expr(value, ctx)?;
                writeln!(self.out, ";")?
            }
            // Stores a value into an image.
            Statement::ImageStore {
                image,
                coordinate,
                array_index,
                value,
            } => {
                // This will only panic if the module is invalid
                let dim = match *ctx.typifier.get(image, &self.module.types) {
                    TypeInner::Image { dim, .. } => dim,
                    _ => unreachable!(),
                };

                write!(self.out, "imageStore(")?;
                self.write_expr(image, ctx)?;
                write!(self.out, ", ")?;
                self.write_texture_coordinates(coordinate, array_index, dim, ctx)?;
                write!(self.out, ", ")?;
                self.write_expr(value, ctx)?;
                writeln!(self.out, ");")?;
            }
            // A `Call` is written `name(arguments)` where `arguments` is a comma separated expressions list
            Statement::Call {
                function,
                ref arguments,
            } => {
                write!(self.out, "{}(", &self.names[&NameKey::Function(function)])?;
                self.write_slice(arguments, |this, _, arg| this.write_expr(*arg, ctx))?;
                writeln!(self.out, ");")?
            }
        }

        Ok(())
    }

    /// Helper method to write expressions
    ///
    /// # Notes
    /// Doesn't add any newlines or leading/trailing spaces
    fn write_expr(&mut self, expr: Handle<Expression>, ctx: &FunctionCtx<'_, '_>) -> BackendResult {
        match ctx.expressions[expr] {
            // `Access` is applied to arrays, vectors and matrices and is written as indexing
            Expression::Access { base, index } => {
                self.write_expr(base, ctx)?;
                write!(self.out, "[")?;
                self.write_expr(index, ctx)?;
                write!(self.out, "]")?
            }
            // `AccessIndex` is the same as `Access` except that the index is a constant and it can
            // be applied to structs, in this case we need to find the name of the field at that
            // index and write `base.field_name`
            Expression::AccessIndex { base, index } => {
                self.write_expr(base, ctx)?;

                match *ctx.typifier.get(base, &self.module.types) {
                    TypeInner::Vector { .. }
                    | TypeInner::Matrix { .. }
                    | TypeInner::Array { .. } => write!(self.out, "[{}]", index)?,
                    TypeInner::Struct { .. } => {
                        // This will never panic in case the type is a `Struct`, this is not true
                        // for other types so we can only check while inside this match arm
                        let ty = ctx.typifier.get_handle(base).unwrap();

                        write!(
                            self.out,
                            ".{}",
                            &self.names[&NameKey::StructMember(ty, index)]
                        )?
                    }
                    ref other => return Err(Error::Custom(format!("Cannot index {:?}", other))),
                }
            }
            // Constants are delegated to `write_constant`
            Expression::Constant(constant) => {
                self.write_constant(&self.module.constants[constant])?
            }
            // `Compose` is pretty simple we just write `type(components)` where `components` is a
            // comma separated list of expressions
            Expression::Compose { ty, ref components } => {
                self.write_type(ty)?;

                write!(self.out, "(")?;
                self.write_slice(components, |this, _, arg| this.write_expr(*arg, ctx))?;
                write!(self.out, ")")?
            }
            // Function arguments are written as the argument name
            Expression::FunctionArgument(pos) => {
                write!(self.out, "{}", ctx.get_arg(pos, &self.names))?
            }
            // Global variables need some special work for their name but
            // `get_global_name` does the work for us
            Expression::GlobalVariable(handle) => {
                let global = &self.module.global_variables[handle];
                write!(self.out, "{}", self.get_global_name(handle, global))?
            }
            // A local is written as it's name
            Expression::LocalVariable(handle) => {
                write!(self.out, "{}", self.names[&ctx.name_key(handle)])?
            }
            // glsl has no pointers so there's no load operation, just write the pointer expression
            Expression::Load { pointer } => self.write_expr(pointer, ctx)?,
            // `ImageSample` is a bit complicated compared to the rest of the IR.
            //
            // First there are three variations depending wether the sample level is explicitly set,
            // if it's automatic or it it's bias:
            // `texture(image, coordinate)` - Automatic sample level
            // `texture(image, coordinate, bias)` - Bias sample level
            // `textureLod(image, coordinate, level)` - Zero or Exact sample level
            //
            // Furthermore if `depth_ref` is some we need to append it to the coordinate vector
            Expression::ImageSample {
                image,
                sampler: _, //TODO
                coordinate,
                array_index,
                offset: _, //TODO
                level,
                depth_ref,
            } => {
                //TODO: handle MS

                //Write the function to be used depending on the sample level
                let fun_name = match level {
                    crate::SampleLevel::Auto | crate::SampleLevel::Bias(_) => "texture",
                    crate::SampleLevel::Zero | crate::SampleLevel::Exact(_) => "textureLod",
                    crate::SampleLevel::Gradient { .. } => "textureGrad",
                };
                write!(self.out, "{}(", fun_name)?;

                // Write the image that will be used
                self.write_expr(image, ctx)?;
                // The space here isn't required but it helps with readability
                write!(self.out, ", ")?;

                // We need to get the coordinates vector size to later build a vector that's `size + 1`
                // if `depth_ref` is some, if it isn't a vector we panic as that's not a valid expression
                let size = match *ctx.typifier.get(coordinate, &self.module.types) {
                    TypeInner::Vector { size, .. } => size,
                    _ => unreachable!(),
                };

                let mut coord_dim = size as u8;
                if array_index.is_some() {
                    coord_dim += 1;
                }
                if depth_ref.is_some() {
                    coord_dim += 1;
                }

                // Compose a new texture coordinates vector
                write!(self.out, "vec{}(", coord_dim)?;
                self.write_expr(coordinate, ctx)?;
                if let Some(expr) = array_index {
                    write!(self.out, ", ")?;
                    self.write_expr(expr, ctx)?;
                }
                if let Some(expr) = depth_ref {
                    write!(self.out, ", ")?;
                    self.write_expr(expr, ctx)?;
                }
                write!(self.out, ")")?;

                match level {
                    // Auto needs no more arguments
                    crate::SampleLevel::Auto => (),
                    // Zero needs level set to 0
                    crate::SampleLevel::Zero => write!(self.out, ", 0")?,
                    // Exact and bias require another argument
                    crate::SampleLevel::Exact(expr) | crate::SampleLevel::Bias(expr) => {
                        write!(self.out, ", ")?;
                        self.write_expr(expr, ctx)?;
                    }
                    crate::SampleLevel::Gradient { x, y } => {
                        write!(self.out, ", ")?;
                        self.write_expr(x, ctx)?;
                        write!(self.out, ", ")?;
                        self.write_expr(y, ctx)?;
                    }
                }

                // End the function
                write!(self.out, ")")?
            }
            // `ImageLoad` is also a bit complicated.
            // There are two functions one for sampled
            // images another for storage images, the former uses `texelFetch` and the latter uses
            // `imageLoad`.
            // Furthermore we have `index` which is always `Some` for sampled images
            // and `None` for storage images, so we end up with two functions:
            // `texelFetch(image, coordinate, index)` - for sampled images
            // `imageLoad(image, coordinate)` - for storage images
            Expression::ImageLoad {
                image,
                coordinate,
                array_index,
                index,
            } => {
                // This will only panic if the module is invalid
                let (dim, class) = match *ctx.typifier.get(image, &self.module.types) {
                    TypeInner::Image {
                        dim,
                        arrayed: _,
                        class,
                    } => (dim, class),
                    _ => unreachable!(),
                };

                let fun_name = match class {
                    ImageClass::Sampled { .. } => "texelFetch",
                    ImageClass::Storage(_) => "imageLoad",
                    // TODO: Is there even a function for this?
                    ImageClass::Depth => todo!(),
                };

                write!(self.out, "{}(", fun_name)?;
                self.write_expr(image, ctx)?;
                write!(self.out, ", ")?;
                self.write_texture_coordinates(coordinate, array_index, dim, ctx)?;

                if let Some(index_expr) = index {
                    write!(self.out, ", ")?;
                    self.write_expr(index_expr, ctx)?;
                }
                write!(self.out, ")")?;
            }
            // Query translates into one of the:
            // - textureSize/imageSize
            // - textureQueryLevels
            // - textureSamples/imageSamples
            Expression::ImageQuery { image, query } => {
                // This will only panic if the module is invalid
                let (dim, class) = match *ctx.typifier.get(image, &self.module.types) {
                    TypeInner::Image {
                        dim,
                        arrayed: _,
                        class,
                    } => (dim, class),
                    _ => unreachable!(),
                };
                let components = match dim {
                    crate::ImageDimension::D1 => 1,
                    crate::ImageDimension::D2 => 2,
                    crate::ImageDimension::D3 => 3,
                    crate::ImageDimension::Cube => 2,
                };
                match query {
                    crate::ImageQuery::Size { level } => {
                        match class {
                            ImageClass::Sampled { .. } | ImageClass::Depth => {
                                write!(self.out, "textureSize(")?;
                                self.write_expr(image, ctx)?;
                                write!(self.out, ",")?;
                                if let Some(expr) = level {
                                    self.write_expr(expr, ctx)?;
                                } else {
                                    write!(self.out, "0",)?;
                                }
                            }
                            ImageClass::Storage(_) => {
                                write!(self.out, "imageSize(")?;
                                self.write_expr(image, ctx)?;
                            }
                        }
                        write!(self.out, ").{}", &"xyz"[..components])?;
                    }
                    crate::ImageQuery::NumLevels => {
                        write!(self.out, "textureQueryLevels(",)?;
                        self.write_expr(image, ctx)?;
                        write!(self.out, ")",)?;
                    }
                    crate::ImageQuery::NumLayers => {
                        let selector = ['x', 'y', 'z', 'w'];
                        let fun_name = match class {
                            ImageClass::Sampled { .. } | ImageClass::Depth => "textureSize",
                            ImageClass::Storage(_) => "imageSize",
                        };
                        write!(self.out, "{}(", fun_name)?;
                        self.write_expr(image, ctx)?;
                        write!(self.out, ",0).{}", selector[components])?;
                    }
                    crate::ImageQuery::NumSamples => {
                        // assumes ARB_shader_texture_image_samples
                        let fun_name = match class {
                            ImageClass::Sampled { .. } | ImageClass::Depth => "textureSamples",
                            ImageClass::Storage(_) => "imageSamples",
                        };
                        write!(self.out, "{}(", fun_name)?;
                        self.write_expr(image, ctx)?;
                        write!(self.out, ")",)?;
                    }
                }
                return Err(Error::Custom("ImageQuery not implemented".to_string()));
            }
            // `Unary` is pretty straightforward
            // "-" - for `Negate`
            // "~" - for `Not` if it's an integer
            // "!" - for `Not` if it's a boolean
            //
            // We also wrap the everything in parentheses to avoid precedence issues
            Expression::Unary { op, expr } => {
                write!(
                    self.out,
                    "({} ",
                    match op {
                        UnaryOperator::Negate => "-",
                        UnaryOperator::Not => match *ctx.typifier.get(expr, &self.module.types) {
                            TypeInner::Scalar {
                                kind: ScalarKind::Sint,
                                ..
                            } => "~",
                            TypeInner::Scalar {
                                kind: ScalarKind::Uint,
                                ..
                            } => "~",
                            TypeInner::Scalar {
                                kind: ScalarKind::Bool,
                                ..
                            } => "!",
                            ref other =>
                                return Err(Error::Custom(format!(
                                    "Cannot apply not to type {:?}",
                                    other
                                ))),
                        },
                    }
                )?;

                self.write_expr(expr, ctx)?;

                write!(self.out, ")")?
            }
            // `Binary` we just write `left op right`, except when dealing with
            // comparison operations on vectors as they are implemented with
            // builtin functions.
            // Once again we wrap everything in parentheses to avoid precedence issues
            Expression::Binary { op, left, right } => {
                // Holds `Some(function_name)` if the binary operation is
                // implemented as a function call
                let function = if let (TypeInner::Vector { .. }, TypeInner::Vector { .. }) = (
                    ctx.typifier.get(left, &self.module.types),
                    ctx.typifier.get(right, &self.module.types),
                ) {
                    match op {
                        BinaryOperator::Less => Some("lessThan"),
                        BinaryOperator::LessEqual => Some("lessThanEqual"),
                        BinaryOperator::Greater => Some("greaterThan"),
                        BinaryOperator::GreaterEqual => Some("greaterThanEqual"),
                        BinaryOperator::Equal => Some("equal"),
                        BinaryOperator::NotEqual => Some("notEqual"),
                        _ => None,
                    }
                } else {
                    None
                };

                write!(self.out, "{}(", function.unwrap_or(""))?;
                self.write_expr(left, ctx)?;

                if function.is_some() {
                    write!(self.out, ",")?
                } else {
                    write!(
                        self.out,
                        " {} ",
                        match op {
                            BinaryOperator::Add => "+",
                            BinaryOperator::Subtract => "-",
                            BinaryOperator::Multiply => "*",
                            BinaryOperator::Divide => "/",
                            BinaryOperator::Modulo => "%",
                            BinaryOperator::Equal => "==",
                            BinaryOperator::NotEqual => "!=",
                            BinaryOperator::Less => "<",
                            BinaryOperator::LessEqual => "<=",
                            BinaryOperator::Greater => ">",
                            BinaryOperator::GreaterEqual => ">=",
                            BinaryOperator::And => "&",
                            BinaryOperator::ExclusiveOr => "^",
                            BinaryOperator::InclusiveOr => "|",
                            BinaryOperator::LogicalAnd => "&&",
                            BinaryOperator::LogicalOr => "||",
                            BinaryOperator::ShiftLeft => "<<",
                            BinaryOperator::ShiftRight => ">>",
                        }
                    )?;
                }

                self.write_expr(right, ctx)?;

                write!(self.out, ")")?
            }
            // `Select` is written as `condition ? accept : reject`
            // We wrap everything in parentheses to avoid precedence issues
            Expression::Select {
                condition,
                accept,
                reject,
            } => {
                write!(self.out, "(")?;
                self.write_expr(condition, ctx)?;
                write!(self.out, " ? ")?;
                self.write_expr(accept, ctx)?;
                write!(self.out, " : ")?;
                self.write_expr(reject, ctx)?;
                write!(self.out, ")")?
            }
            // `Derivative` is a function call to a glsl provided function
            Expression::Derivative { axis, expr } => {
                write!(
                    self.out,
                    "{}(",
                    match axis {
                        DerivativeAxis::X => "dFdx",
                        DerivativeAxis::Y => "dFdy",
                        DerivativeAxis::Width => "fwidth",
                    }
                )?;
                self.write_expr(expr, ctx)?;
                write!(self.out, ")")?
            }
            // `Relational` is a normal function call to some glsl provided functions
            Expression::Relational { fun, argument } => {
                let fun_name = match fun {
                    // There's no specific function for this but we can invert the result of `isinf`
                    RelationalFunction::IsFinite => "!isinf",
                    RelationalFunction::IsInf => "isinf",
                    RelationalFunction::IsNan => "isnan",
                    // There's also no function for this but we can invert `isnan`
                    RelationalFunction::IsNormal => "!isnan",
                    RelationalFunction::All => "all",
                    RelationalFunction::Any => "any",
                };
                write!(self.out, "{}(", fun_name)?;

                self.write_expr(argument, ctx)?;

                write!(self.out, ")")?
            }
            Expression::Math {
                fun,
                arg,
                arg1,
                arg2,
            } => {
                use crate::MathFunction as Mf;

                let fun_name = match fun {
                    // comparison
                    Mf::Abs => "abs",
                    Mf::Min => "min",
                    Mf::Max => "max",
                    Mf::Clamp => "clamp",
                    // trigonometry
                    Mf::Cos => "cos",
                    Mf::Cosh => "cosh",
                    Mf::Sin => "sin",
                    Mf::Sinh => "sinh",
                    Mf::Tan => "tan",
                    Mf::Tanh => "tanh",
                    Mf::Acos => "acos",
                    Mf::Asin => "asin",
                    Mf::Atan => "atan",
                    Mf::Atan2 => "atan2",
                    // decomposition
                    Mf::Ceil => "ceil",
                    Mf::Floor => "floor",
                    Mf::Round => "round",
                    Mf::Fract => "fract",
                    Mf::Trunc => "trunc",
                    Mf::Modf => "modf",
                    Mf::Frexp => "frexp",
                    Mf::Ldexp => "ldexp",
                    // exponent
                    Mf::Exp => "exp",
                    Mf::Exp2 => "exp2",
                    Mf::Log => "log",
                    Mf::Log2 => "log2",
                    Mf::Pow => "pow",
                    // geometry
                    Mf::Dot => "dot",
                    Mf::Outer => "outerProduct",
                    Mf::Cross => "cross",
                    Mf::Distance => "distance",
                    Mf::Length => "length",
                    Mf::Normalize => "normalize",
                    Mf::FaceForward => "faceforward",
                    Mf::Reflect => "reflect",
                    // computational
                    Mf::Sign => "sign",
                    Mf::Fma => "fma",
                    Mf::Mix => "mix",
                    Mf::Step => "step",
                    Mf::SmoothStep => "smoothstep",
                    Mf::Sqrt => "sqrt",
                    Mf::InverseSqrt => "inversesqrt",
                    Mf::Inverse => "inverse",
                    Mf::Transpose => "transpose",
                    Mf::Determinant => "determinant",
                    // bits
                    Mf::CountOneBits => "bitCount",
                    Mf::ReverseBits => "bitfieldReverse",
                };

                write!(self.out, "{}(", fun_name)?;
                self.write_expr(arg, ctx)?;
                if let Some(arg) = arg1 {
                    write!(self.out, ", ")?;
                    self.write_expr(arg, ctx)?;
                }
                if let Some(arg) = arg2 {
                    write!(self.out, ", ")?;
                    self.write_expr(arg, ctx)?;
                }
                write!(self.out, ")")?
            }
            // `As` is always a call.
            // If `convert` is true the function name is the type
            // Else the function name is one of the glsl provided bitcast functions
            Expression::As {
                expr,
                kind,
                convert,
            } => {
                let inner = ctx.typifier.get(expr, &self.module.types);
                if convert {
                    // this is similar to `write_type`, but with the target kind
                    match *inner {
                        TypeInner::Scalar { kind: _, width } => {
                            write!(self.out, "{}", glsl_scalar(kind, width)?.full)?
                        }
                        TypeInner::Vector {
                            size,
                            kind: _,
                            width,
                        } => write!(
                            self.out,
                            "{}vec{}",
                            glsl_scalar(kind, width)?.prefix,
                            size as u8
                        )?,
                        ref other => unreachable!("unexpected cast of {:?}", other),
                    }
                } else {
                    let source_kind = inner.scalar_kind().unwrap();
                    write!(
                        self.out,
                        "{}",
                        match (source_kind, kind) {
                            (ScalarKind::Float, ScalarKind::Sint) => "floatBitsToInt",
                            (ScalarKind::Float, ScalarKind::Uint) => "floatBitsToUInt",
                            (ScalarKind::Sint, ScalarKind::Float) => "intBitsToFloat",
                            (ScalarKind::Uint, ScalarKind::Float) => "uintBitsToFloat",
                            _ => {
                                return Err(Error::Custom(format!(
                                    "Cannot bitcast {:?} to {:?}",
                                    source_kind, kind
                                )));
                            }
                        }
                    )?;
                }

                write!(self.out, "(")?;
                self.write_expr(expr, ctx)?;
                write!(self.out, ")")?
            }
            // A `Call` is written `name(arguments)` where `arguments` is a comma separated expressions list
            Expression::Call {
                function,
                ref arguments,
            } => {
                write!(self.out, "{}(", &self.names[&NameKey::Function(function)])?;
                self.write_slice(arguments, |this, _, arg| this.write_expr(*arg, ctx))?;
                write!(self.out, ")")?
            }
            // `ArrayLength` is written as `expr.length()` and we convert it to a uint
            Expression::ArrayLength(expr) => {
                write!(self.out, "uint(")?;
                self.write_expr(expr, ctx)?;
                write!(self.out, ".length())")?
            }
        }

        Ok(())
    }

    fn write_texture_coordinates(
        &mut self,
        coordinate: Handle<Expression>,
        array_index: Option<Handle<Expression>>,
        dim: crate::ImageDimension,
        ctx: &FunctionCtx,
    ) -> Result<(), Error> {
        match array_index {
            Some(layer_expr) => {
                let tex_coord_type = match dim {
                    crate::ImageDimension::D1 => "ivec2",
                    crate::ImageDimension::D2 => "ivec3",
                    crate::ImageDimension::D3 => "ivec4",
                    crate::ImageDimension::Cube => "ivec4",
                };
                write!(self.out, "{}(", tex_coord_type)?;
                self.write_expr(coordinate, ctx)?;
                write!(self.out, ", ")?;
                self.write_expr(layer_expr, ctx)?;
                write!(self.out, ")")?;
            }
            None => {
                self.write_expr(coordinate, ctx)?;
            }
        }
        Ok(())
    }

    /// Helper method used to produce the images mapping that's returned to the user
    ///
    /// It takes an iterator of [`Function`](crate::Function) references instead of
    /// [`Handle`](crate::arena::Handle) because [`EntryPoint`](crate::EntryPoint) isn't in any
    /// [`Arena`](crate::arena::Arena) and we need to traverse it
    fn collect_texture_mapping(&self) -> Result<FastHashMap<String, TextureMapping>, Error> {
        use std::collections::hash_map::Entry;
        let info = self
            .analysis
            .get_entry_point(self.options.shader_stage, &self.options.entry_point);
        let mut mappings = FastHashMap::default();

        for sampling in info.sampling_set.iter() {
            let tex_name = self.names[&NameKey::GlobalVariable(sampling.image)].clone();

            match mappings.entry(tex_name) {
                Entry::Vacant(v) => {
                    v.insert(TextureMapping {
                        texture: sampling.image,
                        sampler: Some(sampling.sampler),
                    });
                }
                Entry::Occupied(e) => {
                    if e.get().sampler != Some(sampling.sampler) {
                        log::error!("Conflicting samplers for {}", e.key());
                        return Err(Error::ImageMultipleSamplers);
                    }
                }
            }
        }

        for (handle, var) in self.module.global_variables.iter() {
            if info[handle].is_empty() {
                continue;
            }
            match self.module.types[var.ty].inner {
                crate::TypeInner::Image { .. } => (),
                _ => continue,
            }
            let tex_name = self.names[&NameKey::GlobalVariable(handle)].clone();
            if let Entry::Vacant(e) = mappings.entry(tex_name) {
                e.insert(TextureMapping {
                    texture: handle,
                    sampler: None,
                });
            }
        }

        Ok(mappings)
    }
}

/// Structure returned by [`glsl_scalar`](glsl_scalar)
///
/// It contains both a prefix used in other types and the full type name
struct ScalarString<'a> {
    /// The prefix used to compose other types
    prefix: &'a str,
    /// The name of the scalar type
    full: &'a str,
}

/// Helper function that returns scalar related strings
///
/// Check [`ScalarString`](ScalarString) for the information provided
///
/// # Errors
/// If a [`Float`](crate::ScalarKind::Float) with an width that isn't 4 or 8
fn glsl_scalar(kind: ScalarKind, width: Bytes) -> Result<ScalarString<'static>, Error> {
    Ok(match kind {
        ScalarKind::Sint => ScalarString {
            prefix: "i",
            full: "int",
        },
        ScalarKind::Uint => ScalarString {
            prefix: "u",
            full: "uint",
        },
        ScalarKind::Float => match width {
            4 => ScalarString {
                prefix: "",
                full: "float",
            },
            8 => ScalarString {
                prefix: "d",
                full: "double",
            },
            _ => return Err(Error::UnsupportedScalar(kind, width)),
        },
        ScalarKind::Bool => ScalarString {
            prefix: "b",
            full: "bool",
        },
    })
}

/// Helper function that returns the glsl variable name for a builtin
fn glsl_built_in(built_in: BuiltIn) -> &'static str {
    match built_in {
        // vertex
        BuiltIn::Position => "gl_Position",
        BuiltIn::BaseInstance => "gl_BaseInstance",
        BuiltIn::BaseVertex => "gl_BaseVertex",
        BuiltIn::ClipDistance => "gl_ClipDistance",
        BuiltIn::InstanceIndex => "gl_InstanceID",
        BuiltIn::PointSize => "gl_PointSize",
        BuiltIn::VertexIndex => "gl_VertexID",
        // fragment
        BuiltIn::FragCoord => "gl_FragCoord",
        BuiltIn::FragDepth => "gl_FragDepth",
        BuiltIn::FrontFacing => "gl_FrontFacing",
        BuiltIn::SampleIndex => "gl_SampleID",
        BuiltIn::SampleMaskIn => "gl_SampleMaskIn",
        BuiltIn::SampleMaskOut => "gl_SampleMask",
        // compute
        BuiltIn::GlobalInvocationId => "gl_GlobalInvocationID",
        BuiltIn::LocalInvocationId => "gl_LocalInvocationID",
        BuiltIn::LocalInvocationIndex => "gl_LocalInvocationIndex",
        BuiltIn::WorkGroupId => "gl_WorkGroupID",
        BuiltIn::WorkGroupSize => "gl_WorkGroupSize",
    }
}

/// Helper function that returns the string corresponding to the storage class
fn glsl_storage_class(class: StorageClass) -> &'static str {
    match class {
        StorageClass::Function => "",
        StorageClass::Input => "in",
        StorageClass::Output => "out",
        StorageClass::Private => "",
        StorageClass::Storage => "buffer",
        StorageClass::Uniform => "uniform",
        StorageClass::Handle => "uniform",
        StorageClass::WorkGroup => "shared",
        StorageClass::PushConstant => "",
    }
}

/// Helper function that returns the string corresponding to the glsl interpolation qualifier
///
/// # Errors
/// If [`Patch`](crate::Interpolation::Patch) is passed, as it isn't supported in glsl
fn glsl_interpolation(interpolation: Interpolation) -> Result<&'static str, Error> {
    Ok(match interpolation {
        Interpolation::Perspective => "smooth",
        Interpolation::Linear => "noperspective",
        Interpolation::Flat => "flat",
        Interpolation::Centroid => "centroid",
        Interpolation::Sample => "sample",
        Interpolation::Patch => return Err(Error::PatchInterpolationNotSupported),
    })
}

/// Helper function that returns the glsl dimension string of [`ImageDimension`](crate::ImageDimension)
fn glsl_dimension(dim: crate::ImageDimension) -> &'static str {
    match dim {
        crate::ImageDimension::D1 => "1D",
        crate::ImageDimension::D2 => "2D",
        crate::ImageDimension::D3 => "3D",
        crate::ImageDimension::Cube => "Cube",
    }
}

/// Helper function that returns the glsl storage format string of [`StorageFormat`](crate::StorageFormat)
fn glsl_storage_format(format: StorageFormat) -> &'static str {
    match format {
        StorageFormat::R8Unorm => "r8",
        StorageFormat::R8Snorm => "r8_snorm",
        StorageFormat::R8Uint => "r8ui",
        StorageFormat::R8Sint => "r8i",
        StorageFormat::R16Uint => "r16ui",
        StorageFormat::R16Sint => "r16i",
        StorageFormat::R16Float => "r16f",
        StorageFormat::Rg8Unorm => "rg8",
        StorageFormat::Rg8Snorm => "rg8_snorm",
        StorageFormat::Rg8Uint => "rg8ui",
        StorageFormat::Rg8Sint => "rg8i",
        StorageFormat::R32Uint => "r32ui",
        StorageFormat::R32Sint => "r32i",
        StorageFormat::R32Float => "r32f",
        StorageFormat::Rg16Uint => "rg16ui",
        StorageFormat::Rg16Sint => "rg16i",
        StorageFormat::Rg16Float => "rg16f",
        StorageFormat::Rgba8Unorm => "rgba8ui",
        StorageFormat::Rgba8Snorm => "rgba8_snorm",
        StorageFormat::Rgba8Uint => "rgba8ui",
        StorageFormat::Rgba8Sint => "rgba8i",
        StorageFormat::Rgb10a2Unorm => "rgb10_a2ui",
        StorageFormat::Rg11b10Float => "r11f_g11f_b10f",
        StorageFormat::Rg32Uint => "rg32ui",
        StorageFormat::Rg32Sint => "rg32i",
        StorageFormat::Rg32Float => "rg32f",
        StorageFormat::Rgba16Uint => "rgba16ui",
        StorageFormat::Rgba16Sint => "rgba16i",
        StorageFormat::Rgba16Float => "rgba16f",
        StorageFormat::Rgba32Uint => "rgba32ui",
        StorageFormat::Rgba32Sint => "rgba32i",
        StorageFormat::Rgba32Float => "rgba32f",
    }
}
